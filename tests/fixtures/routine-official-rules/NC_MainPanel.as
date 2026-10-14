package mmo.activityext.newact20240705.nationalcompetition.panel
{
   import com.fairygui.GButton;
   import com.fairygui.GComponent;
   import com.fairygui.GObject;
   import mmo.activityext.newact20240705.nationalcompetition.config.NC_Config;
   import mmo.activityext.newact20240705.nationalcompetition.helper.NC_Helper;
   import mmo.activityext.newact20240705.nationalcompetition.model.reward.NC_ChildReward;
   import mmo.activityext.newact20240705.nationalcompetition.model.reward.NC_Reward;
   import mmo.activityext.newact20240705.nationalcompetition.model.reward.NC_YuanHunReward;
   import mmo.activityext.newact20240705.nationalcompetition.panel.components.NC_RankCell;
   import mmo.activityext.newact20240705.nationalcompetition.panel.tips.NC_TipsPanel;
   import mmo.common.DisplayUtil;
   import mmo.common.dialog.common.strategy.PageComponentStrategy;
   import mmo.common.helper.ActUtil;
   import mmo.common.shareobject.LocalStore;
   import mmo.common.shareobject.OnceKeyUtil;
   import mmo.common.sound.SoundManager;
   import mmo.common.unifiedicon.UnifiedIcon;
   import mmo.common.user.UserInfo;
   import mmo.common.utils.dt.redisrank.render.SlideShowRenderer;
   import mmo.common.utils.tips.TipsManager;
   import mmo.interfaces.ServiceContainer;
   import mmo.interfaces.pet.data.IPetCV;
   import mmo.interfaces.pet.data.IPetDictionaryDataService;
   import mmo.interfaces.pet.peticon.PetIconContainer;
   import mmo.interfaces.pet.peticon.PetIconType;
   import mmo.interfaces.strengthencombo.IStrengthenComboService;
   import mmo.play.compatible.H5;
   import mmo.play.globalfunctions.forSvc;
   
   public class NC_MainPanel extends NC_BasePanel
   {
      
      private static const PAGE_REWARD_NUM:int = 6;
      
      private static const PAGE_UNIQUE_REWARD_NUM:int = 4;
      
      private static const BONUS_CONFIG_MAP_FRAME:Array = [11,27,43,59,74,90,100];
      
      private static const MC_LABLE_KEY:String = "NC_MainPanel.MC_LABLE_KEY";
      
      private var _pageRank0:PageComponentStrategy;
      
      private var _pageReward:PageComponentStrategy;
      
      private var _ppt:SlideShowRenderer;
      
      private var _petCV:IPetCV;
      
      public function NC_MainPanel()
      {
         super("main","MainPanel");
      }
      
      private static function getCurBonusMapFrame(param1:Array) : Array
      {
         var _loc2_:Array = BONUS_CONFIG_MAP_FRAME.slice(0,param1.length - 1);
         _loc2_.push(BONUS_CONFIG_MAP_FRAME[BONUS_CONFIG_MAP_FRAME.length - 1]);
         return _loc2_;
      }
      
      private function get curBonusMapScore() : Array
      {
         var scoreArr:Array = null;
         var scoreArrLast:int = 0;
         var mySore:int = model.myScore;
         scoreArr = model.reward.getRewards().map(function(param1:NC_Reward, ... rest):int
         {
            return param1.cost;
         });
         var tempScoreArr:Array = scoreArr.slice(this.getRealIndex(0),this.getRealIndex(0) + PAGE_REWARD_NUM);
         if(this._pageReward.currentPage >= this._pageReward.pageComponent.getTotalPage)
         {
            scoreArrLast = tempScoreArr[tempScoreArr.length - 1] + 1;
         }
         else
         {
            scoreArrLast = Math.round((scoreArr[this.getRealIndex(0) + PAGE_REWARD_NUM] - scoreArr[this.getRealIndex(0) + PAGE_REWARD_NUM - 1]) / 2 + scoreArr[this.getRealIndex(0) + PAGE_REWARD_NUM - 1]);
         }
         tempScoreArr.push(scoreArrLast);
         if(this._pageReward.currentPage > 1)
         {
            mySore -= Math.round((scoreArr[this.getRealIndex(0)] - scoreArr[this.getRealIndex(0) - 1]) / 2 + scoreArr[this.getRealIndex(0) - 1]);
            tempScoreArr = tempScoreArr.map(function(param1:int, ... rest):int
            {
               return param1 - Math.round((scoreArr[getRealIndex(0)] - scoreArr[getRealIndex(0) - 1]) / 2 + scoreArr[getRealIndex(0) - 1]);
            });
         }
         return [Math.max(mySore,0),tempScoreArr];
      }
      
      private function get currentPage() : int
      {
         var _loc1_:NC_Reward = null;
         var _loc2_:int = 1;
         var _loc3_:int = 0;
         while(_loc3_ < model.reward.getRewards().length)
         {
            _loc1_ = model.reward.getRewards()[_loc3_] as NC_Reward;
            if(!_loc1_.hasClaimed)
            {
               _loc2_ = _loc3_;
               break;
            }
            _loc3_++;
         }
         return Math.floor(_loc2_ / PAGE_REWARD_NUM) + 1;
      }
      
      override protected function initClickMap(param1:Object) : void
      {
         super.initClickMap(param1);
         param1["btnRule"] = this.onRule;
         param1["btnUpdate"] = this.onUpdate;
         param1["btnStart"] = this.onStart;
         param1["btnPoster"] = this.onPoster;
         param1["btnBuyTicket"] = this.onBuyTicket;
         param1["~_I~btnBonus"] = this.onGetBonus;
         param1["~T~btnGet"] = this.onGetUniqueBonus;
         param1["~__I~btnEvo"] = this.onEvoUniquePetBonus;
         param1["~_I~btnDetail"] = this.onShowDetail;
         param1["~_I~btnPetCV"] = this.onCv;
         param1["~T~btnSkinEffect"] = this.onEffect;
      }
      
      override protected function initComponents() : void
      {
         super.initComponents();
         model.updateSignal.add(this.update);
         this._pageReward = new PageComponentStrategy(model.reward.getRewards(),this.rewardChangeFunc,PAGE_REWARD_NUM);
         this._pageReward.init(getChildEx("spBonusCom"));
         this._pageReward.setCurrentPage(this.currentPage);
         this._pageRank0 = new PageComponentStrategy(null,null,NC_Config.NUM_PER_PAGE,this.createGetServerDataWrapFun);
         this._pageRank0.setCellRenderer(NC_RankCell);
         this._pageRank0.init(getChildEx("spRankCom"));
         setTheChild(["spRankCom","spTopReward","spContainer"],NC_Config.RANK_TOP_TITLE == "" ? new UnifiedIcon(NC_Config.RANK_TOP_REWARD).setSize(60).asCom : new UnifiedIcon(NC_Config.RANK_TOP_TITLE,true,false).setSize(60).asCom);
         this.updatePanel();
      }
      
      override protected function afterAddToStage() : void
      {
         super.afterAddToStage();
         if(!OnceKeyUtil.getKey("newact20240705.nationalcompetition.panel.NC_MainPanel"))
         {
            this.onUpdate();
         }
      }
      
      private function update() : void
      {
         this._pageReward.updateCurrentPage();
         this.updatePanel();
      }
      
      private function createGetServerDataWrapFun(param1:int, param2:int, param3:Function) : void
      {
         var startIdx:int = param1;
         var num:int = param2;
         var cacheAndRenderCallback:Function = param3;
         manager.tryGetRankData(this._pageRank0.currentPage,function(param1:Object):void
         {
            param1["rs"] = param1["s"];
            param1["my"] = param1["mr"];
            cacheAndRenderCallback(param1);
         });
      }
      
      private function rewardChangeFunc(param1:Array) : void
      {
         if(this._ppt != null)
         {
            this._ppt.dispose();
         }
         if(!LocalStore.getValue(MC_LABLE_KEY,UserInfo.userName) && this._pageReward.currentPage != 1)
         {
            LocalStore.setValue(MC_LABLE_KEY,true,UserInfo.userName);
            setVisible("mcBonusLable",false);
         }
         setText("spBonusCom.txtMyScore",model.myScore);
         var _loc2_:Array = this.curBonusMapScore;
         var _loc3_:int = int(ActUtil.getProgressBarFrameByConfig(_loc2_[0],_loc2_[1],getCurBonusMapFrame(_loc2_[1])));
         setProgressBar("spBonusCom.mcPrg",_loc3_);
         var _loc4_:int = 0;
         while(_loc4_ < PAGE_REWARD_NUM)
         {
            setVisible("spBonusCom.spBox_" + _loc4_,_loc4_ < param1.length);
            if(_loc4_ < param1.length)
            {
               this.setRewardBox(param1[_loc4_],getChildEx("spBonusCom.spBox_" + _loc4_),45);
            }
            _loc4_++;
         }
      }
      
      private function setRewardBox(param1:NC_Reward, param2:GComponent, param3:int) : void
      {
         var petIdsArr:Array = null;
         var isSkin:Boolean = false;
         var fristReward:NC_ChildReward = null;
         var reward:NC_Reward = param1;
         var container:GComponent = param2;
         var width:int = param3;
         petIdsArr = [];
         var isPet:Boolean = false;
         isSkin = false;
         var spContainer:GComponent = getChildEx([container,"spContainer"]);
         DisplayUtil.clearContainer(spContainer);
         TipsManager.getInstance().removeTips(spContainer);
         reward.possessedRewards.forEach(function(param1:NC_ChildReward, ... rest):void
         {
            if(param1.isPetPrize)
            {
               petIdsArr.length < 3 && petIdsArr.push(param1.getPetPrize().raceId);
               isPet = true;
               if(!isSkin)
               {
                  isSkin = param1.getPetPrize().isSkin;
               }
            }
         });
         if(isPet)
         {
            this.initPetIcon(spContainer,petIdsArr);
            TipsManager.getInstance().addTips(spContainer,isSkin ? "指定精灵皮肤任选1" : "指定精灵任选1");
         }
         else if(reward.isTimesReward)
         {
            spContainer.addChild(new UnifiedIcon("120:200109:" + reward.childRewardTimes,false).setSize(45).asCom);
            TipsManager.getInstance().addTips(spContainer,reward.basicDescription);
         }
         else if(!reward.isPetPrize && Boolean(reward.isTypeFusion))
         {
            fristReward = reward.possessedRewards[0];
            spContainer.addChild(new UnifiedIcon(fristReward.rewardParams,false).setSize(45).asCom);
            TipsManager.getInstance().addTips(spContainer,reward.basicDescription);
         }
         else
         {
            reward.setRewardBox(container,Boolean(reward.isTypeCombo) && !reward.isSameCombo ? 90 : 45);
         }
         setText([container,"txtCost"],reward.cost);
         setVisible([container,"spClaimed"],reward.hasClaimed);
         setVisible([container,"btnBonus"],!reward.hasClaimed && model.myScore >= reward.cost);
         setFrame([container,"bg"],Boolean(reward.isTypeCombo) && !reward.isSameCombo ? 2 : 1);
         setVisible([container,"spLight"],!reward.hasClaimed && model.myScore >= reward.cost);
         setFrame([container,"spLight"],Boolean(reward.isTypeCombo) && !reward.isSameCombo ? 2 : 1);
         if(Boolean(reward.isTypeCombo) && !reward.isSameCombo)
         {
            spContainer.x = 20;
            spContainer.y = -15;
         }
         else
         {
            spContainer.x = 43;
            spContainer.y = 10;
         }
      }
      
      private function initPetIcon(param1:GComponent, param2:Array) : void
      {
         var container:GComponent = param1;
         var petIdsArr:Array = param2;
         this._ppt = new SlideShowRenderer(container);
         this._ppt.setData({
            "interval":24,
            "isMouseOverStop":true,
            "nextTriggerFun":function(param1:int):void
            {
               var _loc4_:* = undefined;
               var _loc5_:* = undefined;
               var _loc2_:* = param1 % petIdsArr.length;
               if(container.numChildren < petIdsArr.length)
               {
                  _loc4_ = new PetIconContainer(petIdsArr[_loc2_],PetIconType.FIGHT,0,0);
                  container.addChild(new GObject(_loc4_));
                  _loc4_.x = _loc4_.x + container.width / 2 + 2;
                  _loc4_.y += container.height / 2;
               }
               var _loc3_:* = 0;
               while(_loc3_ < container.numChildren)
               {
                  _loc5_ = container.getChildAt(_loc3_);
                  _loc5_.visible = _loc3_ == _loc2_;
                  _loc3_++;
               }
            }
         });
      }
      
      private function updatePanel() : void
      {
         setText("spRankCom.txtWinTimes",model.summaryWinTimes);
         setText("spRankCom.txtKingTimes",model.totalTopTimes);
         setText("spTicket.txtTickets",model.weekTicketNum);
         setText("spDailyTicket.txtDailyTickets",Math.max(0,NC_Config.DAILY_MAX_TICKET + model.buyDailyTicketTimes - model.dailyTicketNum) + "/" + NC_Config.DAILY_MAX_TICKET);
         setVisible("spTicket.btnBuyTicket",model.weekTicketNum <= 0);
         setVisible("spDailyTicket.btnBuyTicket",model.dailyTicketNum >= NC_Config.DAILY_MAX_TICKET + model.buyDailyTicketTimes);
         if(this._pageReward.currentPage == 1 && !LocalStore.getValue(MC_LABLE_KEY,UserInfo.userName))
         {
            setVisible("mcBonusLable",true);
         }
         else
         {
            setVisible("mcBonusLable",false);
         }
         this.upadteUniqueBonus();
      }
      
      private function upadteUniqueBonus() : void
      {
         var _loc1_:String = null;
         var _loc2_:NC_Reward = null;
         var _loc4_:GComponent = null;
         var _loc3_:int = 0;
         while(_loc3_ < model.uniqueReward.length)
         {
            _loc1_ = "spUniqueBonusCom.spBox_" + _loc3_;
            _loc2_ = model.uniqueReward[_loc3_];
            if(_loc2_.isTimesReward)
            {
               setFrame(_loc1_ + ".mcBtn",!this.yunHunUniqueState ? 2 : 1);
               this.yunHunUniqueState && setVisible(_loc1_ + ".mcBtn.btnGet",model.myYuanHunTimes > 0);
            }
            else
            {
               if(Boolean(_loc2_.isTypePet) && Boolean(_loc2_.getPetPrize().justOpen))
               {
                  setFrame(_loc1_ + ".mcBtn",3);
               }
               else
               {
                  setFrame(_loc1_ + ".mcBtn",_loc2_.hasClaimed ? 2 : 1);
               }
               !_loc2_.hasClaimed && setVisible(_loc1_ + ".mcBtn.btnGet",model.myScore >= _loc2_.cost);
               if(Boolean(getChildEx(_loc1_ + ".spContainer")))
               {
                  _loc4_ = getChildEx(_loc1_ + ".spContainer");
                  _loc2_.setRewardBox(getChildEx(_loc1_),_loc4_.width);
               }
            }
            _loc3_++;
         }
      }
      
      public function get yunHunUniqueState() : Boolean
      {
         return model.essenceReward.getRewards().some(function(param1:NC_YuanHunReward, ... rest):Boolean
         {
            return !param1.hasClaimed;
         });
      }
      
      private function onBuyTicket() : void
      {
         manager.tryBuyTicket(false,this.updatePanel);
      }
      
      private function onGetBonus(param1:int) : void
      {
         var reward:NC_Reward = null;
         var index:int = param1;
         reward = model.reward.getRewards()[this.getRealIndex(index)];
         if(reward.isPetPrize)
         {
            manager.showPetSelectPanel(reward);
         }
         else if(reward.isTypeFusion)
         {
            manager.showEssenceSelectPanel(reward);
         }
         else
         {
            manager.tryGetBonus(reward,function():void
            {
               _pageReward.updateCurrentPage();
               if(reward.isTimesReward)
               {
                  manager.showEssenceSelectPanel();
               }
            });
         }
      }
      
      private function onGetUniqueBonus(param1:GButton) : void
      {
         var reward:NC_Reward = null;
         var target:GButton = param1;
         reward = model.uniqueReward[parseInt(target.parent.parent.name.split("_")[1])];
         if(reward.isTimesReward)
         {
            manager.showEssenceSelectPanel();
         }
         else if(reward.isPetPrize && Boolean(reward.isTypeFusion))
         {
            manager.showPetSelectPanel(reward);
         }
         else if(reward.isTypeFusion)
         {
            manager.showEssenceSelectPanel(reward);
         }
         else
         {
            manager.tryGetBonus(reward,function():void
            {
               _pageReward.updateCurrentPage();
               updatePanel();
            });
         }
      }
      
      private function onEvoUniquePetBonus(param1:int) : void
      {
         var index:int = param1;
         var reward:NC_Reward = model.uniqueReward[index];
         if(reward == null || !reward.isTypePet || !reward.getPetPrize().justOpen)
         {
            return;
         }
         reward.getPetPrize().applyOpen(function():*
         {
            _pageReward.updateCurrentPage();
            updatePanel();
         });
      }
      
      private function onShowDetail(param1:int) : void
      {
         var reward:NC_Reward = null;
         var index:int = param1;
         reward = model.uniqueReward[index];
         if(reward.isTypeStrengthen)
         {
            ServiceContainer.tryGetService(IStrengthenComboService,function(param1:IStrengthenComboService, param2:Object):void
            {
               param1.showPetList(reward.filter);
            });
         }
         else if(reward.isTimesReward)
         {
            manager.showEssenceSelectPanel(null,true);
         }
         else if(reward.isPetPrize)
         {
            manager.showPetSelectPanel(reward,true);
         }
         else if(reward.isTypeFusion)
         {
            manager.showEssenceSelectPanel(reward,true);
         }
      }
      
      private function onCv(param1:int) : void
      {
         var reward:NC_Reward = null;
         var index:int = param1;
         reward = model.uniqueReward[index];
         SoundManager.instance.stopBackgroundMusic();
         this.removeCurrentCV();
         forSvc(IPetDictionaryDataService,function(param1:IPetDictionaryDataService):void
         {
            removeCurrentCV();
            _petCV = param1.getPetCV(reward.getPetPrize().raceId);
            _petCV.play(-1);
         });
      }
      
      private function removeCurrentCV() : void
      {
         if(this._petCV != null)
         {
            this._petCV.stop();
            this._petCV = null;
         }
      }
      
      private function onPoster() : void
      {
         manager.showTipsPanel(NC_TipsPanel.P_SKIN_POSTER_TIPS);
      }
      
      private function onEffect(param1:GButton) : void
      {
         var _loc2_:NC_Reward = model.uniqueReward[parseInt(param1.parent.name.split("_")[1])];
         _loc2_.getPetPrize().showInfoOrEffect(manager.showMainPanel);
      }
      
      private function onRule() : void
      {
         manager.showTipsPanel(NC_TipsPanel.P_RULES_TIPS);
      }
      
      private function onUpdate() : void
      {
         manager.showTipsPanel(NC_TipsPanel.P_UPDATE_TIPS);
      }
      
      private function onStart() : void
      {
         if(model.state != NC_Config.STATE_NOT_START)
         {
            onClickClose();
            manager.showPanel(true);
            return;
         }
         if(model.weekTicketNum <= 0 || model.dailyTicketNum >= NC_Config.DAILY_MAX_TICKET + model.buyDailyTicketTimes)
         {
            manager.tryBuyTicket(true,function():void
            {
               updatePanel();
               ActUtil.applyCallback(onStart);
            });
            return;
         }
         if(model.state != NC_Config.STATE_NOT_START)
         {
            onClickClose();
            manager.showPanel(true);
            return;
         }
         manager.tryRandomFormation(this.playOpenMovie);
      }
      
      private function playOpenMovie() : void
      {
         var handlesyc:Function = null;
         handlesyc = function():void
         {
            ActUtil.lockScreen(false);
            onClickClose();
            manager.showPanel();
         };
         ActUtil.lockScreen(true);
         H5.call(function():void
         {
            NC_Helper.loadPlaySpineMovie("cc01","1",handlesyc);
         },function():void
         {
            NC_Helper.loadPlayMovieByContainer("openmovie","OpenMovie",getChildEx("openMvContainer"),handlesyc);
         });
      }
      
      private function getRealIndex(param1:int) : int
      {
         return (this._pageReward.currentPage - 1) * PAGE_REWARD_NUM + param1;
      }
      
      override public function dispose() : void
      {
         model.updateSignal.remove(this.update);
         if(this._pageReward != null)
         {
            this._pageReward.dispose();
         }
         if(this._pageRank0 != null)
         {
            this._pageRank0.dispose();
         }
         if(this._ppt != null)
         {
            this._ppt.dispose();
         }
         this.removeCurrentCV();
         super.dispose();
      }
   }
}

